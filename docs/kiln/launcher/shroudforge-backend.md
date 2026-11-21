# Settings: the ShroudForge card

The **ShroudForge** card in the launcher's **Settings** (between the kiln.json card and the Graphics card) chooses which
backend runs this game's ShroudForge mods. The choice is per game, saved in `<game>/kiln/kiln.json` as
`"shroudforge": { "backend": ... }`, and applies from the next start. What each backend does:
[shroudforge.md](../shroudforge.md) (bundled and installed) and [shroudforge-kiln-mode.md](../shroudforge-kiln-mode.md)
(kiln mode).

## The choices

| Choice on the card | kiln.json value | What runs your ShroudForge mods |
|---|---|---|
| **Kiln's bundled ShroudForge** (Default) | `hosted`, or nothing set | The runtime that comes with Kiln. |
| **My installed ShroudForge** | `installed` | The copy in the game's `shroudforge` folder. |
| **Kiln's interpreter** | `kiln` | Kiln itself (kiln mode). A mod it cannot run whole is skipped, with the reason, before Play. |

## What the card shows

- **Setting**: "Nothing set: the default.", "Chosen: ...", or, when the older switch is still in kiln.json, "From the
  older switch "Use my installed ShroudForge": ...". Saving a choice replaces that switch (`useInstalled` is removed).
  A value Kiln does not know is shown as such: no ShroudForge runs this game's ShroudForge mods until you choose one.
- **Next start** and **Last start**: what the next start will use, and what the last start actually used.
- **Held**: choosing **Kiln's interpreter** needs no other setting. When this Kiln build does not offer kiln mode, it is
  greyed with "Not available in this Kiln build: <reason>". Anything that needs ShroudForge 1.8 is never offered.
- **Warnings**: "Kiln's interpreter is not in this game yet: install Kiln again to add it."; "ShroudForge's own loader
  is in the game (<file>): it runs the ShroudForge mods, and Kiln's choices stand aside."; and notes when
  `KILN_SHROUDFORGE_RUNTIME` or `KILN_SHROUDFORGE=0` is set where the launcher runs.

## Changing it

1. Close the game (while it runs the card says "Close the game to change this.").
2. Pick a choice. A dialog shows what changes from the next start; for **Kiln's interpreter** it reminds you that mods
   it cannot run whole are skipped and listed in the warning before Play, and that nothing switches back on its own.
3. **Save** writes the one key. **Cancel** or **Esc** writes nothing. **Back to the default** removes the setting.

The card writes only if kiln.json is still the file it read: if you edited it by hand in the meantime, it refuses
and asks you to look again, and it never writes a kiln.json it cannot read. Each change leaves one line in
`launcher.log`.

The card is for game clients. It does not change a dedicated server's kiln.json.
