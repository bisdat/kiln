# EMF mods on Kiln (kiln-emf)

Kiln loads unmodified mods for **EMF** (Enshrouded Modding Framework, MIT; API versions through EMF 0.6.19 / v47)
through the plugin `kiln/plugins/kiln-emf.dll`. It supplies an `EmfApi` v47 table built on Kiln's services:
of 144 C slots, 80 are full, 19 partial, and 45 (functions EMF added between 0.6.3 and 0.6.19)
answer "not available on Kiln yet" ([what that means for a mod](#mods-built-for-emf-063-to-0619)). v41, v42 and v45
(EMF 0.6.10, 0.6.11 and 0.6.14) added no function. API coverage is not a guarantee for every mod
pairing or player-facing feature. Every function and its evidence: [emf-parity.md](emf-parity.md). You keep your EMF
DLL mods and EMF's Lua mods; you do not need EMF's own `dinput8.dll` or `emf_core.dll`.

Evidence, design and the mapping of each function: [docs/re/emf-compat.md](../re/emf-compat.md).

## Install a mod

1. Install Kiln as usual. `tools/deploy-server.sh` copies `kiln-emf.dll` to `kiln/plugins/`.
2. Put the mod where EMF would put it: a folder `<game>/emf/mods/<id>/` with its `mod.json` and DLL, or a single
   `<game>/emf/mods/<name>.dll`. EMF's Lua mods also need the `lua` mod (`emf/mods/lua/`).
   With the Kiln Launcher, a profile's `emf` entries are loaded too (`kiln.profile`). The `emf/mods` folder still
   counts while the profile has EMF drop-ins on, which is the default.
   EMF's Lua mods can be added in the launcher too, one folder or EMF's whole zip (its Lua mods are in
   `game/emf/mods/`; its `examples/lua/` and `template_lua/` are left out, as EMF does not install them). EMF's `lua`
   mod finds a Lua mod only by its folder in `emf/mods`, so activation copies each enabled one to
   `<game>/emf/mods/<id>/` with Kiln's marker, with or without EMF's own loader, and the profile names that copy (a
   folder of the same mod you put there yourself is used as it is). EMF's DLL mods still run from the Library.
3. Switch it on. Most shipped EMF mods start switched off. Edit `<configDir>/emf/<id>.ini`:
   ```ini
   [big_stacks]
   enabled=1
   multiplier=4
   max=9999
   ```
   `configDir` is `<game>/kiln/config`, or the active profile's `configDir`. kiln-emf creates the file at the first
   start, with every setting at its `mod.json` default. If EMF ran in this folder before, the first start takes the
   values from EMF's own `<game>/emf/config/<id>.ini`.
4. Start the server or the game. `kiln/logs/kiln.log` has one line per mod. Each mod logs under `[emf:<id>]`, for
   example `[emf:big_stacks] big_stacks 1.0.0 (big_stacks\, legacy): loaded in 1.4 ms`. Every mod's status is also in
   `kiln/state/emf.json`.

### Settings
- **Where values live:** `<configDir>/emf/<id>.ini` holds them, in section `[<id>]`. `enabled=0/1` is the mod's
  switch and applies at the next start. The other keys are the mod's settings.
- **The mirror:** kiln-emf copies the section to `<game>/emf/config/<id>.ini`, the file EMF uses. Shipped mods read
  their switch and values there directly (big_stacks, sleep, herald, clock). Edit the Kiln file, not the mirror: the
  mirror is rewritten at each start.
- **Validation:** values are checked as EMF checks them. Out of range is clamped when read from the file and refused
  when set by a command. A choice is stored as its index, a select as the option's value, and a colour as `#rrggbb`.
- **Schema for the launcher:** `<configDir>/.schema/emf/<id>.json` describes the settings (no values), as
  `docs/kiln/launcher/contracts.md` 5.5 defines.
- **Changing values while the game runs:** use the commands `emf.set <mod> <key> <value>` and
  `emf.settings <mod>` (below). A mod that marks a setting `live` sees the change at once.

### Which mods load
- **Switch:** `enabled` in the ini decides. Without a value, `mod.json`'s `"enabled"` decides (default on). A
  profile entry with `enabled: false` is always off.
- **API version:** `"emf": N` above 47 is refused ("needs EMF API vN"). Older API prefixes are accepted; individual mods remain subject to the compatibility limits below.
- **Side:** `"side": "client"` mods do nothing on a dedicated server, so the server skips them (clock). Everything
  else loads everywhere, as in EMF.
- **Dependencies:** `depends` / `dependencies` take EMF's version ranges (`^1.2`, `~1.2`, `>=1.0 <2`, `1.2.x`,
  `1.2 - 2`, `||`). A mod whose required dependency is missing, off, too old or failed does not load, and the log
  says why. Mods load in dependency order, then by folder name. Since 0.2.0-dev.30.2 the launcher says so before Play,
  from the mod.json, with kiln-emf's own rules (`crates/kiln-manage/src/emf_deps.rs`, the profile preview's `emf`): the
  mod's row reads "Needs EMF's lua mod >=1.36, which isn't installed." For EMF's own mods (its `lua` mod and the nine
  Lua mods of its download) it adds that they come with EMF's release zip, to add with Add mods, and links EMF's page
  (github.com/matenm/emf-enshrouded); for another mod it opens the https link the dependency entry gives (`"url"`, which
  EMF ignores), if any; "Get EMF" always opens EMF's page. After a start, a mod kiln-emf rejected or failed
  (`kiln/state/emf.json` of that run) says so on its row when the preview has nothing to add, except for a reason the
  preview judges again and no longer finds (a dependency not installed, switched off in the profile, or out of range).
  EMF's ini switch, the EMF API version and a dependency that did not load are not part of the check, so their
  reasons stay. With EMF's own loader installed the launcher says nothing before Play: kiln-emf steps aside and EMF
  loads emf/mods by its own switches.
- **Script mods:** a `mod.json` with `"main"` instead of `"dll"` is a script mod. The `lua` mod runs it (status
  `script`).
- **Faults:** a mod whose `emf_mod_load` returns false or faults is marked `failed`. Its DLL stays loaded: Kiln never
  unloads an EMF mod.

<a id="mods-built-for-emf-063-to-0611"></a>

<a id="mods-built-for-emf-063-to-0614"></a>

### Mods built for EMF 0.6.3 to 0.6.19
A mod built with EMF's SDK does not start on a host whose table is smaller than the one it was built with. Kiln
served EMF 0.6.0's table until 0.2.0-dev.26, so every EMF DLL built since 0.6.3 stayed off ("this mod was built for
EMF API v40; the core is v27"), and with EMF's `lua` mod every Lua mod of a current EMF. Since 0.2.0-dev.27 the table
is EMF 0.6.8's and they start.

EMF 0.6.10 (API v41) and 0.6.11 (v42) only raised the version: v41 gave move orders `stuck_seconds` and the flag
`EMF_MOVE_PEACEFUL_ALL`, v42 gave path results the door each point goes through (`EmfPathResult.doors`); the table
stayed 1088 bytes. Since 0.2.0-dev.28 kiln-emf says version 42, so a mod whose `mod.json` says `"emf": 41` or `42`
loads instead of being refused ("needs EMF API v42; kiln-emf provides v40"), and `emf::has_v41()` and `has_v42()`
answer true. The functions those fields belong to, `enemy_move` and `path_find`, are still among the ones Kiln does not
carry: a move order of either size (128 or 136 bytes) is refused with its reason, and no path result reaches a mod.

EMF 0.6.13 (API v43) and 0.6.14 (v44 to v46) grew the table to 1144 bytes: the game's own permissions of a player
(`player_permissions`), breaking player-built blocks (`break_blocks`, `block_at`, `find_wall`; v45 spares the world's
own ruins) and villager moves (`villager_move`, `villager_move_cancel`, `villager_moving`). A mod built with EMF
0.6.14's SDK, among them EMF's `lua` mod 1.39 (and with it every Lua mod), Night Raids 1.2 and Villager Jobs 1.5.2,
stayed off on Kiln's 1088-byte table ("this mod was built for EMF API v46; the core is v42: update EMF (the mod stays
off)"). kiln-emf serves that prefix, so that check lets them start (not yet run on Kiln), and a `mod.json`
that says `"emf": 43` to `46` loads. The
block queries and queued sphere/column breaking now translate onto `kiln.build` (since 0.2.0-dev.30.6).
`player_permissions` and villager moves still give their documented unavailable answers. Night Raids can query
building walls and player-placed terrain through the adapter; its real gameplay needs the check below.

EMF 0.6.19 (API v47) appends `find_wall_ex` and `spawn_facing` (1160 bytes). The whole table is served so the SDK's
size/version check accepts Lua 1.40, Night Raids 1.3.0 and Beastmaster 1.19.0, which otherwise report "built for EMF
API v47; the core is v46". Their gameplay has not been checked. `spawn_facing` forwards yaw and extra components
through `kiln.spawn` at creation, not a later teleport. `block_at` returns cell facts including natural/player/unknown
provenance and raw material hardness/health;
`find_wall_ex(..., EMF_FIND_TERRAIN)` includes only player-placed terrain. Breaks never remove natural/unknown terrain,
even with `EMF_BREAK_WORLD`. Damage uses native building cracks and caller-configured terrain health ×250 / 20-second
healing. `EMF_BREAK_DROPS` still refuses the whole request (id 0, the documented unavailable reason, no callback).
`EMF_MARKER_BAR` remains unavailable. Kiln clears exactly selected cells, unlike EMF's possible isolated-terrain
cleanup just outside the box. Ruin protection still shrinks building spheres as EMF does; terrain keeps the original
radius and its own natural/unknown-cell protection. See
[the v47 table](emf-parity.md#v47-emf-0619).

Kiln does not carry every function EMF added since. Carried: chat lines without a speaker (`chat_tell_ex`,
`chat_broadcast_ex`; on Kiln every line a mod sends shows without a name, also a plain one), `world_to_screen` and,
since 0.2.0-dev.30.3, hit observers (`on_hit`, Lua `emf.on_hit`: every hit the server applies to a creature, player
or placed object arrives after the server's step, as on EMF; fall, shroud, cold and drowning damage do not arrive on
Kiln yet, and the mod's log says so once). Not yet: player join/leave/death events, item names and the game's
language, character names, changing hits (`on_hit_before`, hit rules), buffs, markers, map clicks and map actions, canvases and the
minimap's parts, `remove_building`, paths, doors, enemy orders and move orders, a player's game permissions
and villager moves. A mod that calls one of those gets
EMF's documented "not available" answer, as on a game build EMF does not know: the rest of the mod runs. `kiln.log`
says once per mod and function `emf: <mod> called <fn>, not supported (not available on Kiln yet)`, and
`kiln/state/emf.json` lists the calls under `unsupported`.

What that means for EMF's own mods of 0.6.8, read from their source (not seen with a player):
- **minimap** draws nothing (`emf.canvas`, `emf.player_view` and `emf.map_markers` are not carried).
- **Beastmaster**: its markers sent to players (`emf.send_marker`), its buff reads and removals (`emf.buffs`,
  `emf.remove_buff`) and its reads of `emf.player_view` and `emf.enemy_moving` give nothing; the rest runs as
  before.
- **teleports**: the map action (Shift and hold on the map) is not offered and it cannot look the ground up for a
  spot (`map_action`, `ground_at`); its commands work as before.

### When EMF itself is installed
- **kiln-emf steps aside** when EMF's real core runs in the same process: `emf_core.dll` is loaded, or EMF's loader
  was chained by Kiln's proxy (`kiln/chain.ini [chain]`, `dinput8-chain.ini`). The log says
  `kiln-emf steps aside: ...`. Then EMF loads the EMF mods, never both. EMF reads only `emf/mods`: while its loader
  is installed, the launcher copies the profile's enabled EMF mods there as `emf/mods/<id>/` with Kiln's marker at
  activation, and removes those copies again when the loader is gone
  ([engines/README.md](engines/README.md#the-profiles-mods-while-the-own-loader-runs)). Under the real EMF, EMF's own
  switch `emf/config/<id>.ini` (`enabled=`) decides whether such a mod starts.
- **The older EMF 0.3.3 core went idle on the tested game build.** It reports `a required signature is
  missing: EMF stays idle`. kiln-emf notices this about 2 s in and logs how to hand the mods back. Put `;` in front of EMF's line in `kiln/chain.ini` (or remove it from `dinput8-chain.ini`), then
  restart. EMF's files can stay in `emf/`; only its loader must not run.

### What a mod costs
`kiln/state/emf.json` lists every mod with its status and reason, the time its callbacks take per server step
(`tickMsPerStep`, `tickMsMax`), the commands it registered, and the functions it called that Kiln does not support.
In the lab, server-admin cost about 1.1 ms per step and the lua mod with 8 scripts about 0.8 ms. Every other shipped
mod cost 0.1 ms or less.

kiln-emf never waits for the disk on the server world's step (since 0.2.0-dev.29.17): `emf.json`, the mods' settings
files and `emf/commands.txt` are written and read on a thread of its own. When its part of a step still takes over
100 ms, `kiln.log` names the part (`slow tick: kiln-emf's server Pre took ...`, at most once a minute); a file that took
the disk thread over a second is logged too (`disk: ... took ... ms`).

## Commands
EMF mods register commands (`tp`, `herald.say`, `raids.status`, ...). On Kiln they run from two places:
- **Players' chat:** `/<command> args`. The line is consumed, so nobody else sees it. Replies come back as chat lines
  only that player sees (`[EMF] ...`).
  - **Who may run them:** commands a mod marks for players (`EMF_COMMAND_PLAYERS`: `/home`, `/unstuck`,
    `/players`, `/sleep.status`, ...) are for everyone. Every other command is for admins only.
  - **Admins:** players with the game's admin flag, accounts listed in `[chat] admins=0x...` in
    `<configDir>/emf/emf_core.ini` (EMF's own `<game>/emf/emf_core.ini` is read when that file is missing), or
    everyone with `everyone=1`.
  - `[chat] deny=...` takes players' commands away from non-admins. Non-admins may send at most 5 commands in 10 s.
- **The console file:** `<game>/emf/commands.txt`, one command per line. It runs as the server owner, and replies go
  to `kiln.log` as `[cmd] ...`.

Built-ins: `emf.help [page]`, `emf.mods`, `emf.settings <mod>`, `emf.set <mod> <key> <value>`, `chat.players` (in
`commands.txt` also without the `emf.` prefix). `/help` stays Kiln's own command.

## Compatibility by mod
The following table preserves an earlier lab snapshot on svn 1285601, with a copy of a real world and no players
(`tools/emf/lab-check.py`). "Loads" means `emf_mod_load` returned true. "Works" means the
player-free check from its source passed. It is not the current API gap list; use [emf-parity.md](emf-parity.md).
The dev.20 mixed-server checks loaded Lua 1.19, Beastmaster 1.13 and ShroudForge in two boots, including the bear/beasts
asset packs. They do not establish taming, custom portrait display or joined multiplayer. Current messaging and
offline portrait evidence: [emf-network.md](emf-network.md), [emf-portraits.md](emf-portraits.md).
What only a joined player can show is in the client checklist below.

**EMF 0.6.8 on the v40 table** (0.2.0-dev.27, a lab server on Steam build 26430253, no players): EMF 0.6.8's own
mods, built from EMF's source with Kiln's toolchain against EMF's v40 SDK (EMF's released binaries were not used),
start on kiln-emf with no EMF installed. `lua` 1.33.0 loaded and runs 8 Lua mods (Beastmaster 1.17.1, Bounty Board
1.2.0, Champion 1.1.0, Grave Keeper 1.0.0, Hand-over 1.1.1, Kill Counter 1.0.0, Life Skills 1.1.0, Necromancer 1.2.0);
big_stacks, event_log, herald 1.0.1, raids 1.1.0, sample_mod, sleep and teleports 1.1.1 loaded and answered their
commands as in the table below; clock and minimap (client mods) were skipped by the server. teleports asked for
`map_action` and got "not available on Kiln yet". The probe passed 96 checks, the mod messages probe 14, the Kiln-side
cross-check 13; no callback faulted. With the earlier 728-byte table each of those DLLs refused to start. What a
player sees of them is not checked.

| Mod | Loads | Works (player-free evidence) | Limitation at that earlier check |
|---|---|---|---|
| big_stacks | yes | Raised 2252 items x4 (at most 9999). Kiln's own content registry agrees: wood logs 250 → 1000, 0 items off the formula. The stack guard is on. | - |
| server-admin | yes | Its file ops all work: `ping` (api 21), `items` (4213, wood max stack 1000), `settings.set` with read-back, `settings.reset`, an out-of-range refusal, `save` acknowledged through `on_save`. It publishes `state.json` with 38 world settings. | - |
| herald | yes | `players` answers, `herald.say` broadcasts, and `on_chat` is on. | - |
| sleep | yes | `sleep.status` answers, and `game_build` reports a known build, so it does not go idle. | - |
| teleports | yes | All 7 commands are registered (`/home`, `/unstuck` for players), and `tp.pos` answers. The teleport itself is verified on a villager by the probe. | - |
| clock | client: yes (server: skipped, side `client`) | In a client, offline: the time of day in the bottom-right corner through `kiln.ui`, following the game's clock (03:13 → 03:16 while Kiln's probe logged 3.18 h → 3.29 h). | - |
| lua | yes | Runs 8 Lua mods; `lua.status` lists them. | - |
| beastmaster, bounty, champion, grave, handover, kill_counter, life_skills, necromancer (Lua) | yes (script) | Running under `lua`. Their commands answer from the console ("could not tell which player you are"). | wheel (client) |
| quick_stack | yes | `qs.list` answers. Chest moves need a player. | wheel (client) |
| raids | yes | `raids.status` / `raids.world on` answer, and its program registered. At nightfall with raids on it logged `nightfall: a raid was due, but nobody is at a base` (time of day, kept per world). | - |
| loot_scale | yes | `hook_pattern` on the game's loot factor is installed as a link in Kiln's chain. | - |
| loot_magnet, bulk_craft, companion, villager_jobs | yes | Ready, and their mod programs (`emit_event`) registered. They act on a player's input. | wheel (client) |
| emf_probe (Kiln's test mod) | yes | 72 checks over the whole table, 0 failures (96 with the v28 to v40 answers in 0.2.0-dev.27). | - |

**Mods kiln-emf runs differently on purpose** (`native/plugins/emf/src/compat_ticks.h`, logged at load):

- **Expeditions (GronGM) up to 1.2.x:** its `on_tick` is called for the server world only. EMF (and kiln-emf) run a
  mod's `on_tick` after every world's step, each on that world's own thread. Expeditions returns at once for the
  client and prediction worlds, but first clears the world pointer its server-world work reads through, so in a
  game that hosts its own world a main-thread tick lands in the middle of a survivor search and empties it ("No free
  survivors nearby" next to the survivors, Send's "too far away"; seen on a Steam Deck, confirmed with a diagnostic
  build). Its other worlds' ticks do nothing else, so nothing is lost. Versions after 1.2.90 get every world's tick
  again.

## Hook order
`hook`, `hook_pattern` and `hook_address` put each mod's detour into [kiln.hooks v2](services/hooks.md): one physical
detour per game function, shared with Kiln's own services, and your detour is a **link** in it, owned by `emf:<mod id>` and
labelled with the signature name (`state/hooks.json` lists it). `*original` is the link's continuation: it runs the rest of
the chain and then the game's function. As EMF documents, a later hook on a function runs before an earlier one; mods'
links also run before Kiln's own links on that function. Kiln's services read the game's call site with `caller()`, so
your hook in front of them cannot confuse them. The log says `hook <name> at exe+0x... (in Kiln's hook chain)`.

A target outside the game exe (another DLL's code, through `hook_address`) is not in kiln.hooks' reach: it still gets a
plain detour and the log says `outside the game exe`. `kiln-emf`'s own hooks (ChatLogAdd and ClientOutboxFlush for the
v27 client chat bridge) are links too, `ChatLogAdd` after `kiln.chat`'s.

## Compatibility by function
The full table for EmfApi v47 (144 functions, and EMF's Lua API) is [emf-parity.md](emf-parity.md); v28 to v40 are
in [its own table there](emf-parity.md#v28-to-v40-emf-063-to-068), v43 to v46 in
[another](emf-parity.md#v43-to-v46-emf-0613-to-0614), and [v47 here](emf-parity.md#v47-emf-0619). v22-v27 in short:
player level and XP through `kiln.progression`; `change_component` and E-prompt offers through kiln-emf's own
`kiln.world` program; mod windows and pictures through `kiln.ui` in the game client (refused on a dedicated server, as
in EMF). API v27 adds mod messaging on the official `#!emf1` wire; joined-client handshake and gameplay still need checks.
The v21 prefix, with current implementations:

EmfApi v21 has 73 functions. **Full** means it behaves as EMF documents. **Partial** means the differences are
listed. **No** means it logs `emf: <mod> called <fn>, not supported` once and returns a safe failure.

| Function | Status | On Kiln |
|---|---|---|
| `log` | full | `kiln.log` under `[emf:<mod>]` |
| `server_world`, `client_world`, `world_kind`, `world_tick`, `ecs_time_ns`, `server_session`, `local_player` | full | core worlds/session, `kiln.client` |
| `singleton` | full | core singleton map |
| `get_component` | full | `kiln.world` (server), the game's GetComponent (client worlds) |
| `get_template_component` | full | the game's GetTemplateComponent; the size comes from the game's reflection |
| `for_each_entity` | full | the storage's entity map (layout from the side's sigdb, as `kiln.world` derives it); client worlds not yet checked in game |
| `on_tick`, `run_on_world` | full | Post of each world's step (server: after `WaitWorldJobs`) |
| `on_event` | partial | One `kiln.world` program per event type and world kind: the server world, and with `EMF_WORLD_CLIENT` the client world (since 0.2.0-dev.29, delivered after the client world's step on the main thread; not yet run in a game: check `programs.client-emf`, which delivers NotifyEnvFxEvent and HitEvent and checks the payload and thread, and an absent event's refusal). EntityDiedEvent arrives only near a player. |
| `register_program`, `register_program_def` | partial | `kiln.world` system programs: the server world; `EMF_WORLD_CLIENT` through `register_program_world` into every client world (since 0.2.0-dev.29, not yet run in a game: check `programs.client-emf` with the probe mods/tests/emf-client-test). Refused at the call on a dedicated server and where `world.programs.client` is off, as before. `program_write_other` answers nothing in a client-world program (kiln.world's other-entity write marks for the save and replication). |
| `emit_event` | full | the game's emitter, as `kiln.gameplay` uses it |
| `read_events` | full | multiple declared event inputs through `kiln.world` programs |
| `program_singleton`, `program_read_other` | full | `kiln.world` iterator slots |
| `program_write_other` | full | declared other-entity writes through `kiln.world` programs |
| `hook_program` | full | program-update pre/post callbacks through `kiln.types` |
| `resolve`, `hook` | full | EMF signature name → Kiln sigdb symbol (42 of 51 resolve on the server, 47 on the client; 4 `Post*` client names are open) |
| `find_pattern`, `hook_pattern`, `hook_address` | full | on-disk scan; the detour is a [kiln.hooks v2](services/hooks.md) link owned by `emf:<mod id>` (see [Hook order](#hook-order)) |
| `write_memory` | full | private data pages only, never code |
| `game_build` | full | `known` = Kiln verified this exe; `number` = its svn |
| `for_each_item`, `find_item`, `item_info` | full | `kiln.items` catalog, live max stack from `kiln.content` |
| `item_data` | full | `kiln.content` entry (2088 bytes) |
| `set_item_max_stack` | full | `kiln.content` layer of the mod (stack guard on) |
| `put_items`, `put_items_ex`, `take_items` | full | `kiln.items` transactions |
| `give_item` | full | `kiln.spawn` pickup 1 m above the player |
| `spawn`, `destroy` | full | `kiln.spawn` (not saved, except villagers) |
| `spawn_ex` | full | spawn-time extra components through `kiln.spawn` |
| `attribute_get`, `attribute_add` | partial | health, stamina and mana through `kiln.combat`; XP/level keep queued progression timing and request limits |
| `damage` | full | `kiln.combat` hit (light reaction, kill credit) |
| `teleport` | full | `kiln.world` teleport. `NOTIFY_READY` for players: READY 1 s after MOVED. |
| `chat_tell`, `chat_broadcast` | partial | `kiln.chat` server lines (30 lines / 10 s per mod). They show as their text alone; on EMF a plain line shows under the player's own name. |
| `player_name`, `on_chat` | full | `kiln.chat` |
| `register_command`, `register_command_ex`, `command_reply`, `command_sender` | full | players' chat via `kiln.chat` + `emf/commands.txt` |
| `on_save`, `on_load`, `request_save` | full | `kiln.saves` events / request |
| `world_setting_get/set/reset/info`, `on_world_setting_changed` | full | `kiln.settings` (short names, minutes; `persist` per world slot) |
| `setting_*`, `on_setting_changed`, `mod_info`, `mod_info_at` | full | kept by kiln-emf (`<configDir>/emf/<id>.ini`) |
| `hud_set`, `notify` | partial | `kiln.ui` overlay on the client (`hud_set` verified in game with clock; `notify` not yet seen); a server keeps HUD lines and logs notes |
| `wheel_open` | full | radial wheel through `kiln.ui`; host checks pass, client drawing/input still need a player |

## Client checklist (needs a joined player)
- A mod of a current EMF runs with no EMF installed: put EMF 0.6.19's `lua` mod (1.40.0) and a Lua mod (Kill Counter) in
  `emf/mods`, start through Kiln, and kill something: the counter's line shows. `kiln.log` has
  `[emf:lua] ... loaded` and no `built for EMF API` line.
- Turned spawns: on a disposable world, ask a v47 mod to spawn a door/target at yaw 0 and 90 degrees with the
  same extra components. Check both the visible facing and a creature's interaction with the target, not just its
  drawn transform. Check terrain queries and no-drops breaks with the guided block-family check below; marker bars
  still log their unavailable answer without changes.
- The mod check on join: join a server that runs EMF 0.6.8 with `[net] mods_check=refuse` and a mod with a channel,
  with the same mods in your own game. You stay connected after 60 s; `kiln.log` has `EMF mods check: against the
  server's mods: same mods`. With a mod missing on your side that the server requires, one note names it.
- A system line (`chat_tell_ex` with `EMF_CHAT_SYSTEM`, a Lua `emf.tell(..., {system = true})`) shows as its text
  alone, also on a server with text chat off.
- The clock shows the time of day in the bottom-right corner (`hud_set` through `kiln.ui`, client and
  hosted server world ticks, `singleton` on the client world).
- Notes (`notify`) appear at the top of the screen.
- Players' chat commands: `/home`, `/unstuck` (teleports), `/players` (herald). Replies arrive as `[EMF] ...` lines.
- Admin commands are refused for a non-admin account, with the account hash in the log.
- The herald MOTD and join/leave lines; `on_chat` sees players' lines.
- `EntityDiedEvent` mods: herald death lines, companion, server-admin graves, and a kill counted by kill_counter.
- `put_items` / `take_items` into a player inventory (server-admin `inventory.add` / `inventory.remove`), and
  `give_item` drops.
- Teleporting a player: server-admin `teleport`, `tp.pos`, and READY after the destination loads.
- loot_magnet, bulk_craft, quick_stack and villager_jobs while a player plays. Their wheels (`wheel_open`) use kiln.ui's radial
  wheel since dev.20: see the wheel items below.
- Radial wheel (client, a world you host): open a mod's wheel (a Lua `emf.wheel`, e.g. quick_stack's). It shows in the
  middle with its title; the pointer lights the sector it is in and the camera does not move; a left click on a lit entry
  picks it (the mod's callback runs, log line); Esc and a right click cancel (callback with -1). With a mod whose wheel
  has a hold key: holding the key, moving to an entry and letting go picks; letting go in the middle cancels; a quick tap
  leaves it open for a click. With more than 8 entries the mouse wheel turns the page. "Name (details)" labels show the
  details on a second line. While it is open a mod window under it gets no clicks, and `window_open` is refused.
- Window right clicks (v24, client): right-click a row or a button with a key in a mod window (beastmaster's Beasts window
  rows): the mod's callback gets `EMF_UI_RIGHT_CLICK` with the widget's key (log line); a left click still sends `EMF_UI_CLICK`.
- Stamina and mana of a real player: `attribute_get` / `attribute_add` of `EMF_ATTR_STAMINA` / `EMF_ATTR_MANA` on a joined
  player (a mod that spends mana, or `emf.attribute_add`): the bar follows (the lab shows stamina on a creature only).
- v24 windows (client, a world you host): mark a prop as a stable with beastmaster's `beastmaster.stable mark`, look
  at it and press E: the Beasts window opens with its rows; a click on a row reaches the mod; Esc and the X close it;
  the mouse returns to the game.
- v26 pictures: a mod window with a picture (`image_load` of a PNG in the mod's folder) draws it; a folder of frames
  with `fps` turns.
- v25 E prompts: a marked stable (beastmaster) shows the game's "E <verb>" prompt within 32 m; pressing E runs the mod's callback (log line), for the host and a joined player.
- v22: blessings' level reward (`set_level`) queues the level for the next step; nonzero XP towards that level follows
  after the result, several steps later (see [parity limits](emf-parity.md)). Given XP
  levels the player up with the game's own level-up message.

## Guided block-family check (since 0.2.0-dev.30.6)

Use a disposable copied world and EMF Lua 1.40 or a small C API test mod; only the owner runs this check. Record
`kiln.log`, `state/emf.json.blocks` and the `build.*` capability rows. Do not use a valued save for destructive checks.

1. In a callback on the host/server world, query `block_at` on air, an unpainted player building block, a natural
   ruin, natural ground and Stone/dirt placed where the shipped world had air. Expect kind/material/fill, raw
   hardness/health and `world` 0/1/-1 as appropriate; paint 0 must not make a building empty. A joined-only client
   cannot perform server queries or breaks.
2. Trace the same line with `find_wall` and `find_wall_ex(..., EMF_FIND_TERRAIN)` (Lua's `{terrain = true}`). The first
   ignores terrain; the second stops at the first player-terrain cell before the wall, but passes natural ground.
3. Queue an instant sphere and a column without drops. Confirm no edit/callback inside `break_blocks`, at most eight
   spheres run per step, and one result after the executing step on the server thread. With `EMF_BREAK_TERRAIN`, only
   player-placed ground is removed. Natural ground remains with `EMF_BREAK_WORLD` too; nearby ruins are protected
   unless that flag is set. Check excluded sphere corners remain untouched and edits save/reload and replicate.
4. Repeat without instant mode: building cracks advance; player terrain uses material health ×250 and heals after
   20 seconds without a hit (`block_at.damage` remains -1 for terrain). Check Unbreakable cells never go. If a mode's
   capability is unavailable, expect id 0 with a reason, no callback and no partial damage.
5. Ask for drops: expect id 0 and `not available on this game version`, no edit or callback. Queue more than eight
   spheres, then close the world; waiting requests cancel at the next server Post, without editing the next world.
6. Optionally use Night Raids 1.3.0 with a disposable player-block and player-Stone wall: verify wall discovery and
   requests/callbacks. Any unavailable drop request is a refusal, not evidence that a raid broke a wall successfully.

The adapter and Windows stand-in tests do not establish game effects, saving, replication or mod gameplay.
