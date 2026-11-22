# EMF API parity (EmfApi v47)

kiln-emf (`native/plugins/emf`) hands every EMF mod an `EmfApi` table built on Kiln's services. This page lists
every function of EMF 0.6.19's C API (v47, 144 functions, `sdk/include/emf/emf_api.h`; v41, v42 and v45 added none) and every function of its Lua
API (`docs/modding/lua.md`), with the Kiln service behind it, the nearest Kiln C# API, and the status.

- **full**: behaves as EMF documents. **partial**: the difference is in the row. **not yet**: Kiln does not carry
  the function; it logs `emf: <mod> called <fn>, not supported (not available on Kiln yet)` once per mod and returns
  EMF's documented failure (false / 0 / an empty buffer / a reason in the request), as EMF's own core does on a game
  build where the feature is off. A callback handed to such a function is never called.
- **tested**: `probe` = the conformance probe (`mods/tests/emf-test/probe`, lab server), `host` = host test
  (`native/plugins/emf/tests/run.sh`), `client` = only a joined player can show it (client checklist in
  [emf.md](emf.md)), `lab-mod` = a shipped EMF mod uses it in the lab check (`tools/emf/lab-check.py`).
- Counts: **80 full, 19 partial, 45 not yet out of 144.** The v27 prefix has 90 slots (75 full, 15 partial). v28 to
  v40 append 45: one full (`world_to_screen`), three partial (`chat_tell_ex`, `chat_broadcast_ex`, `on_hit` since
  0.2.0-dev.30.3) and 41 not yet
  ([the table below](#v28-to-v40-emf-063-to-068)). v43 to v46 append 7: two full, one partial and four not yet
  ([their table](#v43-to-v46-emf-0613-to-0614)). v47 appends 2, both full (`spawn_facing`,
  `find_wall_ex`; [v47](#v47-emf-0619)). Counts are C table slots, not grouped rows or attribute constants.
  The two attribute slots remain partial because the XP/level ids use the queued progression path below. The four
  chat line slots are partial because a plain line (no `EMF_CHAT_SYSTEM`) shows without a speaker's name on Kiln and
  under the player's own name on EMF (counted as full until 0.2.0-dev.26; the behaviour did not change). Full means
  the implemented contract matches EMF; the evidence column separately names host, server and pending client checks.

API 27 messages use EMF's official `#!emf1` chat wire. Kiln peers can speak to EMF peers without Kiln's own network
protocol. Host checks use the upstream wire peer; the dedicated-server API probe checks the five functions. Two mixed
server boots (dev.20) loaded unmodified Lua 1.19, Beastmaster 1.13 and ShroudForge once each. Those boots do not verify
a joined player's handshake, hidden chat, taming, portraits, UI or multiplayer behaviour. See
[emf-network.md](emf-network.md), which also has the mod check on join (EMF 0.6.3).

## Version negotiation

One table, `version = 47`, `size = sizeof(EmfApi) = 1160`. Slots are only appended (v22 at
byte 592, v23 616, v24 624, v25 648, v26 672, v27 688, v28 728, v29 736, v30 752, v31 776, v32 808, v33 864, v34 920,
v35 952, v36 992, v37 1000, v38 1016, v39 1040, v40 1064; v41 and v42 appended nothing; v43 1088, v44 1096; v45
appended nothing; v46 1120; v47 1144). Every earlier prefix is unchanged. A mod's `mod.json` `"emf": N` above 47 is refused
("needs EMF API vN"). Mods built with EMF's C++ SDK also check
`api->size >= sizeof(EmfApi)` of their own header and do not start on a smaller table, whatever they then call: this
is why Kiln serves the whole table although it does not carry every function yet. A mod built for EMF 0.6.3 or
later, EMF's Lua host among them, did not start on Kiln's earlier 728-byte table; one built for EMF 0.6.13 or 0.6.14
(EMF 0.6.14's Lua host 1.39, Night Raids 1.2 and Villager Jobs 1.5.2 among them) did not start on the 1088-byte v42
table ("this mod was built for EMF API v46; the core is v42"). `emf::has_v45()` goes by the version (v45 did not grow
the table), so the table says 47. The table is a shared, immutable prefix-compatible view; it is not shrunk for an
older mod. EMF 0.6.19 SDK mods (Lua 1.40, Night Raids 1.3.0 and Beastmaster 1.19.0) need the new
1160-byte v47 table, replacing the 1144-byte v46 table that refused them. This is ABI acceptance,
not a gameplay check of those mods.
Structs passed in are read up to their `size` field (EMF's `api_sizes.h` rules): an `EmfUiWidget` array is walked by
`widget_size` (44..4096 bytes, at most 256 widgets), and a v24/v25 mod's 56-byte widgets read `icon`, `fps` and
`height` as 0. A v36 to v46 request (`EmfRemoveBuilding`, `EmfEnemyOrder`, `EmfMoveOrder` for `enemy_move` and
`villager_move`, `EmfBreakBlocks`) whose `size` is smaller than EMF's minimum (144, 120, 128 and 160 bytes) is refused
untouched, as in EMF. v41's `EmfMoveOrder.stuck_seconds` (136 bytes) and v42's
`EmfPathResult.doors` (32 bytes) change nothing on Kiln: `enemy_move` refuses every order and `path_find` never answers
(both not yet). v45's `EmfBlockInfo.world` (40 to 44 bytes) is written only when covered by the caller's size; the block
queries and `EMF_BREAK_WORLD` are translated onto `kiln.build` since 0.2.0-dev.30.6. v46's
`EmfMoveOrder.hold_seconds` (in `pad2`'s place, still 136 bytes) and `EMF_MOVE_HOLD` remain unavailable with
`villager_move`.

## C API

| EMF function (since) | Kiln implementation | Kiln C# equivalent | Status | Tested |
|---|---|---|---|---|
| `log` (1) | `kiln.log` under `[emf:<mod>]` | `Logger` | full | probe |
| `server_world`, `client_world`, `world_kind` (1) | core worlds | `Kiln.World`, `Kiln.Client` | full | probe |
| `world_tick`, `ecs_time_ns` (1) | core tick info | `TickEvents` | full | probe |
| `singleton` (1) | core singleton map | `Kiln.Api` game types (singletons) | full | probe |
| `local_player` (1) | `kiln.client` | `Kiln.Client` | full | probe |
| `get_component` (1) | `kiln.world` component / the game's GetComponent | `World.TryRead<T>` | full | probe |
| `for_each_entity` (1) | the storage's entity map | `World.Find` | full | probe |
| `on_tick`, `run_on_world` (1) | Post of each world's step | `TickEvents` / `Scheduler` | full | probe |
| `resolve`, `hook` (1) | EMF signature name → Kiln sigdb (`sigdb/symbols/*.d/emf.json`) | `Kiln.Hooks` | full | probe |
| `hook_program` (1) | `kiln.types` runner pre/post callbacks | `GameSystems.Hook(name, before, after)` | full | host; server probe |
| `register_program`, `register_program_def` (2/16) | `kiln.world` programs: server world; `EMF_WORLD_CLIENT` through `register_program_world` (since 0.2.0-dev.29, host tests only) | `Kiln.Api` ECS programs | partial: the client world is built and not yet run in a real game (guided check `programs.client-emf`); refused at the call on a dedicated server, as before | probe |
| `spawn`, `destroy` (2) | `kiln.spawn` | `Kiln.Spawn` | full | probe |
| `spawn_ex` (12) | `kiln.spawn` components copied into the spawn request | `SpawnOptions.Components`, `SpawnComponent.Of` | full | host; server probe |
| `on_save`, `on_load` (2) | `kiln.saves` events | `Kiln.Saves` events | full | probe |
| `emit_event` (3) | the game's emitter (as `kiln.gameplay`) | `Kiln.Gameplay` | full | probe |
| `read_events` (3) | `kiln.world` programs v2, multiple event inputs | `Kiln.Api` ECS programs with multiple event inputs | full | host; server probe |
| `on_event` (4) | one `kiln.world` program per event type and world kind (`emf_ev_<Event>`, client world `emf_evc_<Event>`, delivered after that world's step on its thread) | `Events.On<T>` (service events) | partial: the client world is built and not yet run in a real game (guided check `programs.client-emf`) | probe |
| `for_each_item`, `find_item`, `item_info` (5) | `kiln.items` catalog | `Kiln.Items` | full | probe |
| `give_item` (5) | `kiln.spawn` pickup | `Kiln.Items` | full | probe |
| `register_command`, `command_reply` (6) | players' chat (`kiln.chat`) + `emf/commands.txt` | `Kiln.Chat` commands | full | probe, lab-mod |
| `get_template_component` (7) | the game's GetTemplateComponent | `Kiln.World` | full | probe |
| `item_data`, `set_item_max_stack` (7) | `kiln.content` | `Kiln.Content` | full | probe, lab-mod (big_stacks) |
| `write_memory` (7) | private data pages only | `Kiln.Hooks` | full | probe |
| `program_singleton`, `program_read_other` (8) | `kiln.world` iterator slots | `Kiln.Api` ECS programs | full | probe |
| `put_items` (8), `put_items_ex`, `take_items` (11) | `kiln.items` transactions | `Kiln.Items` | full | probe |
| `server_session` (9) | core session | `Kiln.World` | full | probe |
| `wheel_open` (10) | `kiln.ui` radial wheel; server refusal as EMF | `Ui.OpenWheel`, `UiWheel` | full | host; client drawing, mouse/keyboard/gamepad selection pending |
| `notify` (10), `hud_set` (12) | `kiln.ui` overlay | `HudLine` (`Kiln.Ui`) | partial: a server keeps HUD lines and logs notes | client |
| `setting_declare/_int/_float/_text/_set`, `on_setting_changed` (13) | kiln-emf (`<configDir>/emf/<id>.ini`) | `ModConfig` | full | probe |
| `mod_info`, `mod_info_at` (13) | kiln-emf | `ModEntry` | full | probe |
| `find_pattern`, `hook_pattern`, `hook_address` (14) | on-disk scan; the detour is a kiln.hooks v2 raw link owned by `emf:<id>` (newest first, also against Shroudtopia's detours, and before Kiln's own links; outside the exe, or when kiln.hooks has no free target slot: plain safetyhook) | `Kiln.Hooks` | full | probe, lab-mod (loot_scale) |
| `game_build` (14) | Kiln's verified build records | `Diagnostics` | full | probe |
| `command_sender` (15), `register_command_ex` (17) | `kiln.chat` | `Kiln.Chat` | full | probe |
| `program_write_other` (16) | `kiln.world` programs v2, declared other-entity writes | `EcsProgramContext.TryWriteOther<T>` | full | host; server probe |
| `attribute_get`, `attribute_add` (18) | `kiln.combat` health, stamina and mana; v22 ids through `kiln.progression` | `Attributes.Get/Set/Add`, `PlayerProgression` | partial: health/stamina/mana supported; XP/level timing and limits below | probe; host |
| `damage` (18) | `kiln.combat` hit | `Kiln.Combat` | full | probe |
| `chat_tell`, `chat_broadcast` (19) | `kiln.chat` server lines | `Chat.Tell`, `Chat.Broadcast` | partial: the line shows as its text alone, without a speaker's name (on EMF under the player's own name, `Ember: Welcome!`) | probe, lab-mod (herald); client (the line on screen) |
| `player_name`, `on_chat` (19) | `kiln.chat` | `Kiln.Chat` | full | probe, lab-mod (herald) |
| `request_save` (19) | `kiln.saves` | `Kiln.Saves` | full | probe |
| `teleport` (20) | `kiln.world` teleport | `Kiln.World` teleport | full | probe |
| `world_setting_get/_set/_reset/_info`, `on_world_setting_changed` (21) | `kiln.settings` | `Kiln.Settings` | full | probe, lab-mod (server-admin) |
| `player_xp_required` (22) | `kiln.progression` `xp_for_level` (cached per session, any thread) | `PlayerProgression` | full | probe |
| `player_level_cap` (22) | `kiln.progression` | `PlayerProgression` | full | probe |
| `player_level_set` (22) | `kiln.progression` SET_LEVEL + SET_EXPERIENCE | `PlayerProgression` | partial: level queued for the next server step, even in a world callback (EMF: at once there); nonzero XP is a second request after the level result (several steps later), suppressed if superseded or the session/level changed | probe (refusals), host |
| `EMF_ATTR_XP_PENDING` / `_XP` / `_XP_REQUIRED` / `_LEVEL` (22) | `kiln.progression` get / ADD_EXPERIENCE | `PlayerProgression` | partial: pending XP is queued, at most 100,000,000 per call (`kiln.progression`); reads require the server thread | probe (refusals), host |
| `change_component` (23) | kiln-emf's `emf_components` program on `kiln.world` (`queue_add` / `queue_remove`), EMF's list and queue | `Kiln.Api` ECS program `QueueAdd` / `QueueRemove` (low level) | full | probe, host |
| `window_open`, `window_update`, `window_close` (24) | `kiln.ui` ImGui window, capture, keys | `UiWindow` | partial: plain ImGui look; fixed width, content-following height capped at 85% of the frame (scrolls past it) on size-constraint-capable kiln.ui; older UI keeps appearing-only sizing; movable, appearing-only Kiln placement/settings retained rather than upstream pinning; left/right clicks implemented; refused on a dedicated server (as EMF) | probe (server refusal), host, client |
| `spawn_offer` (25) | `kiln.spawn` + the offer below | `Offers.SpawnWithOffer` | partial: the prompt appears a step or two after the spawn | probe |
| `offer`, `offer_remove` (25) | the three offer components through `emf_components`; `InteractionAcceptedEvent` listener | `Offers.Add/TryAdd/Remove` | full | probe (components), host, client (the prompt, the accept) |
| `image_load` (26) | `kiln.ui` `image_load` (PNG frames), EMF's limits and paths; encoded files also limited to 64 MB | `UiImage` | partial: encoded-file limit in addition to EMF's decoded-pixel limit | host, client |
| `offer_active` (25/26) | kiln-emf's offer registry | `Offers.IsActive/Status` | full | probe, host |
| `net_channel` (27) | upstream EMF channel registry, caller ownership and version checks | `Network.Channel` (Kiln wire; not the EMF protocol) | full | host wire/ownership checks; server probe |
| `net_to_server` (27) | EMF wire on host loopback or the game's client chat outbox | `NetChannel.SendToServer` (Kiln wire) | full | host + production client-detour fixture; joined-client check pending |
| `net_to_player` (27) | EMF wire through `kiln.chat`, checked peer/channel | `NetChannel.SendTo(player, message)` (Kiln wire) | full | host upstream-peer interop; server probe; joined-client check pending |
| `net_to_players` (27) | EMF wire to every compatible peer | `NetChannel.SendToAll` (Kiln wire) | full | host; server probe; joined-client check pending |
| `net_peer` (27) | EMF peer/channel-version snapshot | `Network.PeerOf`, `NetPeer.HasChannel` (Kiln peers; presence only) | full | host; server probe |

## v28 to v40 (EMF 0.6.3 to 0.6.8)

What a mod gets from each function EMF added after 0.6.0. "Answer" is the value a mod sees from a function Kiln does
not carry yet. Host tests check every answer (`tests/glue_test.cpp`, `test_v40`); the probe checks them on a
dedicated server.

| EMF function (since) | On Kiln | Status | Tested |
|---|---|---|---|
| `chat_tell_ex`, `chat_broadcast_ex` (30) | `kiln.chat` server lines, which a player's game shows without a speaker's name: with `EMF_CHAT_SYSTEM` as EMF documents. Other flags are refused. | partial: flags 0 sends the same line without a name (on EMF a plain line shows under the player's own name) | probe; client (the line on screen) |
| `world_to_screen` (33) | `kiln.render`'s camera: display pixels, false behind the camera or before a frame. False on a dedicated server. | full | host; client |
| `on_player` (28) | Answer: `false`. No join, leave or death callback comes. | not yet | host, probe |
| `item_name`, `game_language` (29) | Answer: 0 and an empty buffer ("use the debug name"). | not yet | host, probe |
| `player_character_name` (30) | Answer: `false` and an empty buffer. | not yet | host, probe |
| `on_hit` (31) | Since 0.2.0-dev.30.3. The game's HitEvents of the server world, as EMF's core takes them: kiln-emf's own relay program for HitEvent (the one `on_event` uses: a `kiln.world` SYSTEM program, host only) copies them in the step, and after the step, on the tick thread, each becomes EMF's `EmfHit`: attacker = root source, part, target = root target (else the part), amount and stamina (positive = damage), the game's flags (`EMF_HIT_KILLED` on the killing blow), damage types, attack kinds, weapon category, program, impact, reaction, target faction, position; `health_before` -1; `health_after` = `kiln.combat` `health()` of the part, else of the target; `weapon_item` = the weapon entity's ItemState item; `from_mod` = a `damage()` hit (kiln.combat's impact ids, so a Kiln mod's damage counts too). Terrain and building hits are never passed. At most 16 callbacks, null refused; a faulting callback is switched off. A call before the worlds are set up is kept (EMF: any time). | partial: entity hits only: fall, shroud, cold and drowning damage (`EMF_HIT_KIND_FALL` to `_DROWNING`, which EMF reads from its own hooks on four game programs) never arrive; `health_after` of a placed object whose Health is gone is -1 (EMF: the object's maximum). The mod's log says so once, at its first `on_hit`. | host (`tests/hits_test.cpp`); probe (lab: passed 2026-11-21, the Vukah killing blow reached `on_hit` with `EMF_HIT_KILLED`) |
| `on_hit_before`, `hit_rule_add`, `hit_rule_remove` (31) | Answer: `false` / 0. Hits are not changed. | not yet | host, probe |
| `buff_types`, `buff_type`, `buff_text`, `buffs_of`, `buff_apply`, `buff_remove`, `on_buff` (32) | Answer: 0 / `false`; `done` is not called. | not yet | host, probe |
| `marker_set`, `marker_remove`, `marker_clear`, `marker_types`, `marker_send`, `marker_unsend` (33) | Answer: `false` / 0. No marker is drawn or sent. | not yet | host, probe |
| `map_cursor`, `on_map_click`, `map_action`, `ground_at` (34) | Answer: `false`; no callback. | not yet | host, probe |
| `player_view`, `fog_at`, `map_markers`, `canvas_set`, `canvas_draw` (35) | Answer: `false` / 0. No canvas is drawn. | not yet | host, probe |
| `remove_building` (36) | Answer: `false`, `refunded` 0, `why` "not available on Kiln yet". Nothing is removed. | not yet | host, probe |
| `path_find`, `path_cancel` (37) | Answer: 0 / `false`; no callback. | not yet | host, probe |
| `door_state`, `door_set`, `door_lock` (38) | Answer: `EMF_DOOR_NONE` / `false`. | not yet | host, probe |
| `enemy_order`, `enemy_order_cancel`, `enemy_target` (39) | Answer: 0 with `why` in the order / `false` / 0. | not yet | host, probe |
| `enemy_move`, `enemy_move_cancel`, `enemy_moving` (40) | Answer: 0 with `why` in the order / `false` / 0; no callback. | not yet | host, probe |

A mod that registers a callback through a function that is not carried and ignores the `false` runs without that
feature and without an error of its own: the one log line and `state/emf.json` `unsupported` are where it shows.

### Hits on Kiln (v31)

`on_hit` observes; it cannot change a hit (`const EmfHit*`, as in EMF). What it costs: the HitEvent relay is
registered only when an EMF mod is loaded, and while no `on_hit` callback is on it does not read the game's event
queue (one call per server step). A game without EMF mods registers nothing. A joined player's game sees no hits, as
on EMF (the relay is a host-only program).

The record is read at EMF's offsets. At start kiln-emf checks them against the game build: the reflected size of
`HitEvent` (248 bytes) and kiln.combat's sigdb captures `combat/hit.flags` and `combat/hit.health_change`. On a build
where one differs, `on_hit` answers `false`, as on EMF where HitEvent is not usable. `kiln.log` says
`on_hit: the HitEvent layout differs: ...`, and a mod that registered earlier gets `on_hit: no hits will arrive (...)`.

Changing a hit before it lands (`on_hit_before`, the hit rules, and with them `emf.hit_rule`) stays "not yet".
EMF does it inline in the game's applyHitToEntity, on the impact thread, before the health change. Kiln has two
places near it, neither of which fits: `kiln.combat` holds hits for one listener (the managed `Kiln.Combat`; a second
`set_listener` replaces the first) and decides on them a step later, not inline; and its applyHitToEntity link is
kiln.combat's own. Carrying them means a second link on applyHitToEntity through `kiln.hooks` (kiln-emf already owns
raw links for `hook_pattern`), EMF's rule matcher (MIT, `src/core/hits.cpp`, `hit_hook.cpp`) and its once-per-hit walk up a
target's parts, write-back into the HitCandidate (kiln.combat's `HitOffsets`), and a lab check that a cancelled hit
leaves no number, reaction, aggro or credit. Best estimate: 2 to 3 days with host tests and one lab run. Fall, shroud,
cold and drowning damage for `on_hit` would come with it (EMF reads them around four game programs), about 1 more day.

### Buffs (v32): plan

Not carried yet (the answers above). Best estimate for all seven: 4 to 6 days, in this order:

1. `buffs_of` and `on_buff` (about 1 day): `kiln.playerstate` `buffs()` already reads any entity's BuffContainer
   and each buff entity's BuffNetworkData on the server world (type, entity, slot, cleansed, lifetime, end, level).
   `on_buff` = a per-step comparison of each player's buffs (APPLIED, REFRESHED when the same buff entity's end moves
   on, REMOVED with `cleansed`/`replaced`), the first sight saying nothing, as EMF. Open points for the lab:
   `EmfBuff.index` (kiln.playerstate skips empty slots, so the place in the slot group needs one more field), the
   time base of `remaining_ns` (BuffNetworkData's end against the world's ECS time), and client worlds (BuffContainer
   is server only: EMF answers there from replicated data, Kiln would answer 0 at first).
2. `buff_types` and `buff_type` (about 1 day): the BuffRegistry's collection, which `kiln.content`'s buff clones
   already find (`gamedata/GetBuffType`, `gamedata/buffreg.collection`), read for id, slot, apply type, faction
   mask, flags, default time and follow-up; server sigdb entries to add where only the client has them.
3. `buff_remove` (about half a day): the cleansed byte of the target's slot through a kiln.world program's tracked
   write (the game's own cleanse ends it the next step), with `done` after that step.
4. `buff_apply` (2 to 3 days, the risky part): the game's applyBuffCore with a 0xE8-byte ApplyBuffCtx built inside a
   kiln.world program that has the buff programs' access (BuffContainer and LifeTime writes, the buff and balancing
   registries, AppliedBuffEvent out), new signatures on both executables, and a lab check of the effect, HUD icon,
   saving and faction refusals.
5. `buff_text` (half a day to a day): the game's text lookup, shared with v29's `item_name` (not carried either).

Not built in 0.2.0-dev.30.3: step 1 alone is moderate, but its numbers need a lab run first, and steps 2 to 5 call
into the engine.

## v43 to v46 (EMF 0.6.13 to 0.6.14)

EMF 0.6.13 (API v43) and 0.6.14 (v44 to v46) appended seven functions; v45 added none (`EmfBlockInfo.world` and the
flag `EMF_BREAK_WORLD`: without it a break spares the world's own ruins). Since 0.2.0-dev.30.6, the block
family translates onto Kiln's own typed API ([block operations](services/build-blocks.md)); no engine code is
inside kiln-emf. Host tests cover translation, refusals, masks, the queue and callback timing. Gameplay remains
unverified; the probe is non-destructive for these calls.

| EMF function (since) | On Kiln | Status | Tested |
|---|---|---|---|
| `player_permissions` (43) | Answer: `false` and `*perms` 0, as for an entity that is no player. The game's own permissions (`EMF_PERM_*`) are not read; EMF's own admins (`EmfNetFrom.admin`, command senders) are not affected. | not yet | host; probe (not run yet) |
| `break_blocks` (44) | Adapter-owned async queue → `kiln.build.break_box` / `Build.BreakBox`: sphere/column masks, copied material list, damage table, instant/world/player-terrain flags. 64 waiting, FIFO whole columns, at most eight spheres per server step; guarded result callback on server Post, once. Short (<160-byte) requests untouched. Unsupported modes refuse synchronously with EMF's `why`, without mutation or callback. | partial: drops unavailable; exact-cell difference below | host; native Windows stand-in; in-game check pending |
| `block_at`, `find_wall` (44) | `kiln.build.block_at` / `find_wall` (`Build.BlockAt` / `FindWall`), inside a callback on the server world. Raw building id minus 128; fill/paint retained, optional metadata unknown → hardness/health 0, damage -1. DDA in cell order, first building up to 64 m. False leaves output untouched; 40-byte v44 and 44-byte v45+ records retain their size. | full | host; native Windows stand-in; in-game check pending |
| `villager_move` (46) | Answer: 0 with `why` "not available on Kiln yet"; no callback. The same with `hold_seconds` or `EMF_MOVE_HOLD`. An order smaller than 128 bytes, or none, is refused untouched. | not yet | host; probe (not run yet) |
| `villager_move_cancel`, `villager_moving` (46) | Answer: `false` / 0. | not yet | host; probe (not run yet) |

## v47 (EMF 0.6.19)

Two slots are appended: `find_wall_ex` at byte 1144 and `spawn_facing` at 1152; the table is 1160 bytes. The header is
EMF's unchanged MIT header below Kiln's licence preface. No earlier slot or struct size changes: `EmfBlockInfo` stays
44 bytes; `EmfMarker` stays 80 (`bar` at byte 76 replaces `pad2`).

| EMF function / addition | On Kiln | Status | Tested |
|---|---|---|---|
| `spawn_facing` | Same as `spawn_ex`: immediate, inside a callback on the supplied server world, with at most 15 extra components (never `CurrentTransform`). Finite `yaw_deg` is forwarded to `kiln.spawn`'s creation-time transform: degrees about +Y, quaternion `(0, sin(yaw/2), 0, cos(yaw/2))`, no range clamp; 0 keeps the template's own facing. No post-spawn teleport. Bad yaw, world/thread/context, component list or unavailable service: 0, under the calling function's name. | full | host; native Windows stand-in; probe (not run in game) |
| `find_wall_ex` | `kiln.build.find_wall`: flags 0 = buildings; `EMF_FIND_TERRAIN` → `KILN_BUILD_FIND_PLAYER_TERRAIN`, adding only terrain with PlayerPlaced provenance. Natural/unknown terrain passed over; when provenance support is unavailable, the adapter still queries buildings, as EMF does. Unknown flags or any read refusal: false, output untouched. | full | host; native Windows stand-in; in-game check pending |
| `EMF_BREAK_TERRAIN`, terrain `EmfBlockInfo.world`, `hardness`, `health` | Player terrain only, including with `EMF_BREAK_WORLD`; natural/unknown terrain never breaks. `world` maps Natural/PlayerPlaced/Unknown to 1/0/-1; hardness class and raw integer material health match EMF, damage stays -1. Missing qualified metadata/provenance/mode refuses rather than unrestricted editing. | full queries; partial breaking (existing slots) | host; native Windows stand-in; in-game check pending |
| `EMF_MARKER_BAR`, `EmfMarker.bar` | Markers are not carried: `marker_set` false, `marker_send` 0; no bar is drawn or sent. | not yet (existing slots) | host; probe (not run in game) |

### Block-family details and differences (since 0.2.0-dev.30.6)

- EMF owns the queue and callback contract in the adapter, not Kiln core. Calls from any thread queue copied
  requests; server Pre translates each sphere to an x-fastest byte selection mask on Kiln's actual grid, then calls
  `BreakBox`. Results are delivered at server Post, before mod tick subscribers. World closure cancels waiting and
  already-extracted unexecuted work; interrupted columns retain the counts of completed spheres. Like EMF's
  `Dispatcher.post(server)`, their callbacks wait for the next server Post, not the destroying thread.
  A 3-second watchdog cancels waiting work if the runner has not run. Callback faults do not suppress later results.
  A clean unloaded sphere is skipped (DONE with no contribution to counts), as in EMF's `block_glue.cpp:475-478,519`
  and `block_service.cpp:88,115`; a refusal after possible mutation is never treated as that no-op.
- Damage is per hardness class, default 100/100/95/60/50, with the world's mining factor and native building cracks.
  Terrain policy is explicitly supplied by the adapter: material health ×250, healing 20 seconds after the last hit
  (EMF 0.6.19 `src/core/block_rules.h:110,121`, `block_rules.cpp:185-198,201-214`). Queries return **raw** health,
  not ×250. Unknown facts are hardness/health 0 and damage -1, as EMF does. Material health and exchange rate are
  integer fields in upstream `MaterialFacts` (`block_rules.h:22-23`, `block_glue.cpp:767-768,789-790`).
- **Drops remain unavailable.** `EMF_BREAK_DROPS` refuses the **whole** request synchronously: id 0, `why` =
  `not available on this game version`, no callback, no damage or removals. No fabricated zero-yield success.
  A successful no-drops callback reports `dropped = 0`. Other missing requested seams likewise refuse the whole
  request (`EMF_BREAK_TERRAIN` alone uses `breaking terrain is not available on this game version`).
- **Exact cells, not terrain cleanup.** EMF may also clear isolated terrain just outside the box; Kiln clears exactly
  the selected cells. EMF's `VoxelBoxFillAir(grow=0)` disables geometric expansion but still runs isolated-terrain
  cleanup over its widened staging buffer; Kiln uses its guarded writer, readback, save and replication path
  ([qualified engine notes](../re/terrain-edit.md)). Building masks likewise do not invoke the native mining kernel's
  grow=1 boundary expansion.
- **Ruin protection retains EMF's sphere shrinking.** The adapter requests Kiln's generic protected-sphere option
  with the original centre/radius, a 0.01 m margin and a 0.25 m minimum radius (`block_rules.cpp:71-92`). The nearest
  eligible natural/unknown building shrinks the whole building sphere; the terrain pass still uses its full radius.
  `EMF_BREAK_WORLD` bypasses that shrink for buildings, never terrain protection. These are caller policy values,
  not EMF constants baked into Kiln core.
- The adapter explicitly permits edits in bases (`IN_BASES`) for EMF's raid semantics and does not opt into Kiln
  zone checks. Loaded-cell, metadata, provenance and Unbreakable checks still apply; no unrestricted fallback.
  Kiln keeps edited chunks through its existing save-region mechanism, including outside bases.
- A failed writer can have partially committed. The callback is `EMF_BREAK_FAILED`, preserving known/lower-bound
  counts, never retried or compensated. The log says when changes may be partial; further breaks latch off until
  restart after a fault or failed verification. `state/emf.json.blocks` records queue/completion/cancellation/failure
  counts and uncertainty; missing-mode calls remain per-mod in `unsupported`, with per-mod log lines.

## Lua API (`emf.*`, run by EMF's `lua` mod on the C API)

EMF's `lua` mod is an EMF DLL mod built on the public C API only, so every Lua function works on Kiln exactly as far
as the C function under it does. Lua mods need EMF's `lua` mod in `emf/mods/lua/`. Unmodified Lua 1.19 (EMF 0.6.0,
EMF's own DLL, dev.20) and Lua 1.33 (EMF 0.6.8, built from EMF's unchanged source for a lab) load on the v40 table and
run EMF's Lua mods; their actual player-facing features still need the client checklist. Lua 1.39 (EMF 0.6.14) is
built against the v46 header and needs at least a 1144-byte table; it has not been run on Kiln yet.
Lua 1.40 (EMF 0.6.19) needs v47 / 1160 bytes, likewise not run in game.
Lua 1.33 looks at the table's version and size before it uses a newer function, then at the function's answer: on
Kiln the functions still marked not yet below return what EMF's Lua guide documents for a feature that is off (`nil`,
`false`, or `nil` and a reason).

| Lua | C under it | Status |
|---|---|---|
| `emf.id`, `emf.dir`, `emf.log`, `print` | `log`, mod info | full |
| `emf.notify`, `emf.hud` | `notify`, `hud_set` | partial (as C) |
| `emf.on_tick` | `on_tick` | full |
| `emf.bus.subscribe/publish/unsubscribe` | the Lua host itself (`run_on_world`) | full |
| `emf.on_event`, `emf.emits`, `emf.emit` | `on_event`, `register_program_def`, `emit_event` | partial: the client world is built and not yet run in a real game |
| `emf.command`, `emf.reply`, `emf.sender` | `register_command_ex`, `command_reply`, `command_sender` | full |
| `emf.local_player`, `emf.session`, `emf.session_token`, `emf.time_of_day` | worlds, session, singleton | full |
| `emf.on_world_data`, `emf.save_world_data` | `on_save` / `on_load` | full |
| `emf.setting`, `emf.set_setting`, `emf.on_setting_changed`, `emf.on_key`, `emf.key_down` | settings | full (`on_key`: client) |
| `emf.item`, `emf.item_info`, `emf.give`, `emf.put`, `emf.take`, `emf.move` | items | full |
| `emf.spawn`, `emf.destroy` | `spawn_ex`, `destroy` | full; spawn-time components supported |
| `emf.kill`, `emf.health`, `emf.attribute`, `emf.attribute_add`, `emf.damage` | v18 attributes, `damage` | partial: health, stamina and mana supported; XP/level timing and limits as C |
| `emf.xp_required`, `emf.level_cap`, `emf.set_level` | v22 | full / full / partial (queued level, then XP) |
| `emf.add_component`, `emf.remove_component` | `change_component` | full |
| `emf.tell`, `emf.broadcast` | `chat_tell`, `chat_broadcast` | partial (as C: no speaker's name) |
| `emf.on_chat`, `emf.request_save` | v19 | full |
| `emf.teleport` | `teleport` | full |
| `emf.world_settings.get/set/reset/list`, `emf.on_world_setting_changed` | v21 | full |
| `emf.wheel`, `emf.wheel_section`, `emf.wheel_group` | `wheel_open` | full; client input/drawing pending |
| `emf.window`, `emf.window_update`, `emf.window_close`, `emf.image` | v24, v26 | partial (as C) |
| `emf.offer`, `emf.spawn_offer`, `emf.offer_remove`, `emf.offered` | v25, `offer_active` | full / partial / full / full |
| `emf.net.channel(name, def)`, `.send(name, data)`, `.send_to(world, player, name, data)`, `.send_all(world, name, data)`, `.peer(world, player, name)` | v27 `net_channel`, `net_to_server`, `net_to_player`, `net_to_players`, `net_peer` | full implementation; host interop and server probe; joined-client checks pending |
| `world:is_server`, `:is_client`, `:tick`, `:entities`, `:position`, `:get`, `:template`, `:name`, `:account` | worlds, components, `player_name` | full |
| `emf.tell`, `emf.broadcast` with options (`system`) (1.22) | `chat_tell_ex`, `chat_broadcast_ex` | full with `system`; without it partial, as `emf.tell` |
| `emf.world_to_screen` | `world_to_screen` | full (client) |
| `emf.player_data`, `emf.save_player_data` | the Lua host's own files under `emf/config` | full |
| `emf.on_player_joined`, `emf.on_player_left`, `emf.on_player_died` (1.20) | `on_player` | not yet |
| `emf.item_name`, `emf.language` (1.21) | `item_name`, `game_language` | not yet (`emf.item_name` gives the debug name and `false`, `emf.language` `nil`, as on a dedicated server) |
| `emf.on_hit` (1.23) | v31 `on_hit` | partial (since 0.2.0-dev.30.3): entity hits, the default, arrive after the server step with every field of EMF's hit table; `{kinds = ...}` with fall, shroud, cold or drowning gets none of those |
| `emf.hit_rule`, `emf.hit_rule_remove` | v31 `hit_rule_add`, `hit_rule_remove` | not yet (`nil` / `false`) |
| `emf.buff_types`, `emf.buff_type`, `emf.buffs`, `emf.apply_buff`, `emf.remove_buff`, `emf.on_buff` | v32 | not yet |
| `emf.marker`, `emf.remove_marker`, `emf.clear_markers`, `emf.marker_types`, `emf.send_marker`, `emf.unsend_marker` | v33 | not yet |
| `emf.map_cursor`, `emf.on_map_click`, `emf.map_action`, `emf.remove_map_action`, `emf.ground_at` | v34 | not yet |
| `emf.player_view`, `emf.fog_at`, `emf.map_markers`, `emf.canvas`, `emf.canvas_draw` | v35 | not yet |
| `emf.remove_building` | v36 | not yet |
| `emf.path_find`, `emf.path_cancel` | v37 | not yet |
| `emf.door`, `emf.door_open`, `emf.door_close`, `emf.door_lock` | v38 | not yet |
| `emf.enemy_order`, `emf.enemy_order_cancel`, `emf.enemy_target`, `emf.enemy_move`, `emf.enemy_move_cancel`, `emf.enemy_moving` | v39, v40 | not yet |
| `world:permissions` (1.36) | v43 `player_permissions` | not yet |
| `emf.break_blocks`, `emf.block_at`, `emf.find_wall` (1.37; a block's `world` and the break option `world`, 1.38) | v44, v45 | queries full; breaking partial (as C) |
| `emf.villager_move`, `emf.villager_move_cancel`, `emf.villager_moving` (1.39) | v46 | not yet |
| `emf.spawn(..., components, {yaw = degrees})` (1.40) | v47 `spawn_facing` through `kiln.spawn` | full |
| `emf.find_wall(..., {terrain = true})`, `emf.break_blocks` terrain option (1.40) | v47 block additions above | query full; breaking partial (as C) |
| marker `bar` (1.40) | v47 marker addition above | not yet |

## Lifetime and host checks

World teardown closes windows exactly once with `BY_CORE`, `NO_WORLD` and a null world, drops queued clicks and accepts,
and fails pending component changes with a null world. Image handles are cached for the process as EMF specifies;
failed multi-frame loads release already decoded frames, and plugin shutdown releases the cache. Offers only remove
support components they added. Native EMF DLLs remain loaded until process exit; there is no per-mod unload/reload
entry point in kiln-emf.

`native/plugins/emf/tests/run.sh` checks the frozen x64 ABI slot map, model policy/queues and the production window,
progression, offer, event and API 27 network adapters with fake Kiln services (ASan/UBSan). Network checks include
upstream-wire interoperability, fragments, ownership, sender identity, limits, teardown and production client detours
with stand-in engine calls. The world-program review also ran eight unload/reload cycles with no failed checks or
leaked program owners on its server lab. These checks do not demonstrate client rendering or player interaction.

EMF manifest portraits are prepared by an offline CPU renderer in the separate GPL `kiln-eml` component, started
when EMF DLL mods load (on a worker thread; only the thread loading the mods waits in `image_load` for a picture being drawn). The MIT adapter only plans requests and calls that export. Archive/cache checks rendered textured
wolf PNGs and reused them through the Windows DLL ABI. Automatic preparation in a real client, custom client
portraits and their appearance in Beastmaster remain unchecked. See [eml.md](eml.md#emf-creature-portraits).

## What is not in kiln-emf

- **EML content** (`emf_content.dll`, GPL-3.0 in EMF): EMF's loader runs it before the game starts. It is not part of
  the C API above. Kiln's GPL plugin `kiln-eml` covers EML content; kiln-emf stays MIT and does not load it.
- EMF's own console UI, debug bridge, save backups and profile manager are EMF's tooling, not API.
