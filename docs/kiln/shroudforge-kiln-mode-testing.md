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
7. **Switching back and forth.** Choose Kiln's bundled ShroudForge, play, then Kiln's interpreter again: every mod
   runs each time, your mod settings are kept, and no mod shows up twice.

## What to send

- What you tried, what you expected and what happened, with the time if something looked wrong.
- A support report (the launcher's Logs page > Support report), or these two files from the game folder:
  `kiln/logs/kiln.log` and `kiln/state/sfi.json`.
- Send it on GitHub (an issue) or in the Kiln Discord.

If kiln mode gets in your way, choose Kiln's bundled ShroudForge on the ShroudForge card: your mods run as before.

Adapted from ShroudForge by bonsaibauer, MIT.
