# Testing kiln mode

Kiln mode runs ShroudForge-format mods on Kiln's own interpreter. It is new in this release and has not been checked
in a real game yet, so reports from players help. This page lists what to try and what to send.
Background: [ShroudForge mods in kiln mode](shroudforge-kiln-mode.md).

## Before you start

- **Use a copied world** (and a copied character if you test Quiver & Spellbook): some mods change your saves.
- **Turn kiln mode on for one game:** Settings > ShroudForge card > **Kiln's interpreter**. It applies from the next
  start. Kiln's bundled ShroudForge stays the default; you can switch back the same way at any time.
- Keep the mods you normally play with, or try a few ShroudForge mods that run whole in kiln mode, for example
  sf-unlock-blueprints, combat_tweaks, Climb Anywhere, Building Tools and Quiver & Spellbook.

## What to try

1. **The mods load.** Press Play. If a mod cannot run whole, a dialog before Play says why; Cancel is the default.
   In the game, the mods do what they normally do. In the launcher, Mods > Engines shows Kiln's interpreter as running
   and one line per mod.
2. **The mod page opens.** In the game, ShroudForge's menu key (F9 unless you changed it) opens Kiln's ShroudForge
   page with this start's mods and their state; the key closes it again.
3. **Settings stick.** Change one of a mod's settings on that page, close and reopen it: the value is still there,
   and after a restart the mod uses it.
4. **Action buttons work.** A mod's action button says "Requested" and the mod reacts once.
5. **The save warning.** Start once with Quiver & Spellbook on, then switch it off and press Play again: the warning
   before Play names it, and in the game a "do not save" banner, a badge on its row and a marker on the pause menu's
   Mods entry say the same. Do not save that world while the mod is off.
6. **The dialog with the keyboard.** In the dialog before Play, Cancel has the focus; Tab reaches the list and
   Continue; Enter on Cancel, Escape and a click beside the dialog all cancel. Check it at a large text size too.
7. **World reads.** Take the kiln-mode probe from Kiln's repository (`mods/tests/sf-kiln-probe`, copied into
   `<game>/mods`): in a world it writes one line per read to `kiln/logs/kiln.log` (lines with `sf-kiln-probe:`) every
   30 seconds, and again when you press its shortcut (F5) or its **Run probes now** button on Kiln's ShroudForge page.
   Aim with a building tool and move the cursor: its `world.cursor` lines follow you. In your base, its
   `world.props.query` count is the number of props around you (one more when you place one nearby), and its `ecs.query`
   and `ecs.read_bytes` lines answer `ok=true`. Send the lines that say `ok=false`.
8. **Switching back and forth.** Choose Kiln's bundled ShroudForge, play, then Kiln's interpreter again: every mod
   runs each time, your mod settings are kept, and no mod shows up twice.
9. **Mod shortcuts.** Take a mod with a keyboard shortcut, or the keys probe from Kiln's repository
   (`mods/tests/sf-keys-probe`, copied into `<game>/mods`; its shortcut is F4 and each press shows a notice). In the
   world, each press works once, holding the key works once, and with Kiln's menu open the key does nothing. Its
   settings on Kiln's ShroudForge page show the shortcut; change it there, restart, and the new key works.
10. **Building input (a preview).** Mods that build for you through `runtime.world.building` need building input, which
   is held until its own checks pass: add `"preview": ["client.building_input"]` to `kiln/config/kiln.json` to try it.
   Use a copied world: a selected block, a placed and removed block, an undo and a dismantle should each happen once, as
   if you did them, and nothing should happen while you hold your own build button. The checks are in
   [the client pass](client-pass.md#building-input).
11. **Attributes.** Switch sf-unlimited-flight on with only "Unlimited Glider Stamina" on and start again. On a copied
   world you host, gliding should not drain stamina. If the mod's line on Kiln's ShroudForge page shows a reason
   instead, send it. With the kiln-mode probe on too, its `attributes.list` and `attributes.read` lines show what the
   attribute reads answer.
12. **World settings changed by a mod.** On a copied world, switch on SF Survival's "Infinite Weapon Durability" on Kiln's
   ShroudForge page and restart (settings apply at the next start): a weapon's durability no longer drops. Switch it off
   and restart again: it drops as before, because the mod puts the world's own setting back. With the kiln-mode probe
   on too, its `services.get` and `services.read_fields` lines show the setting (`enableDurability`).
13. **Mods that read the game's resources.** On a copied world, host a game in kiln mode with sf-crafting-essentials,
   sf-unlimited-flight, sf-survival or skeleton-commander on, play for a minute, go back to the main menu and enter the
   world again. Nothing freezes or crashes, no mod says the ResourceService is missing (what they still cannot do, they
   name with another reason), and what the mods change (sf-crafting-essentials: its recipe changes) shows in the game
   and is gone again after you switch the mod off and restart. With the kiln-mode probe on too, its
   `resources.by_type` and `resources.get` lines answer `ok=true` in a world.
14. **The voxel grid.** Host a copied world with the voxel probe from Kiln's repository (`mods/tests/sf-voxel-probe`,
   copied into `<game>/mods`; it only reads). Within a few seconds `kiln/logs/kiln.log` has a line
   `probe grid: voxel origin ... cell 0.5,0.5,0.5 maximum 20480,2147483640,20480`, a line `probe read: refused: ...`
   (reading cells is not in kiln mode yet), and the game runs as usual while the probe is on. The kiln-mode probe's
   `voxel.grid_spec` line shows the same grid.
15. **Template components changed by a mod.** On a copied world, switch on SF Survival's "No Fall Damage" on Kiln's
   ShroudForge page and restart: a fall from a height that normally costs health costs none. Switch it off and restart
   again: the same fall costs health as before, because the mod puts the template's own values back.

## What to send

- What you tried, what you expected and what happened, with the time if something looked wrong.
- A support report (the launcher's Logs page > Support report), or these two files from the game folder:
  `kiln/logs/kiln.log` and `kiln/state/sfi.json`.
- Send it on GitHub (an issue) or in the Kiln Discord.

If kiln mode gets in your way, choose Kiln's bundled ShroudForge on the ShroudForge card: your mods run as before.

Adapted from ShroudForge by bonsaibauer, MIT.
