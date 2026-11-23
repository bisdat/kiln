# Vendored: Enshrouded Save Editor

| | |
|---|---|
| Upstream | https://github.com/nicolasferreiraxn/Enshrouded-Save-Editor |
| Commit | `b277f6bafd1b59f675b09204322e3554b598f779` (1.0.2, 2026-10-25, "devolver os icones da arvore de habilidades") |
| Licence | MIT, Nicolas Ferreira ([LICENSE](LICENSE); also `licenses/Enshrouded-Save-Editor-LICENSE.txt`) |
| Copied | `src/`, `shared/`, `data/` |

The editor runs inside the Kiln Launcher at `/editor` (lazy route, `pages/EditorPage.tsx` → `KilnEditor.jsx`). Its
Rust side (upstream `src-tauri/src/main.rs`) is replaced by the launcher's `editor_*` commands
(`launcher/src-tauri/src/commands/editor.rs` on `kiln_manage::saves::editor`, which knows Linux/Proton save paths;
upstream only knew `C:/Program Files*/Steam/userdata`).

## Local changes
Unchanged: `data/*`, `shared/*` except `character.mjs`, and every `src/` file not listed here.

| File | Change |
|---|---|
| `src/main.jsx`, `src/index.html` | removed: the launcher mounts the editor (`KilnEditor.jsx`) |
| `src/Servidor.jsx`, `src/Jogadores.jsx` | removed: the SFTP "Servidor" tab (Guardian server copies) is hidden; the launcher manages servers on its own pages |
| `src/App.jsx` | `TABS` without `srv`; no Servidor/Jogadores render, no "Configurar servidor…" menu entry; the 2560×1440 stage fits the editor's own box (ResizeObserver) instead of the window |
| `src/platform.js` | rewritten: `api` is always `tauri-api.js`; `ASSET_BASE` is the launcher's `kiln-asset` protocol (`kiln-asset://localhost/editor/`, Windows `http://kiln-asset.localhost/editor/`), `/assets-local/editor/` in the browser dev server; `registerFonts` runs once; new `assetsPresent()` |
| `src/tauri-api.js` | its `invoke` names map to the launcher's `editor_*` commands through `launcher/src/api/commands.ts` (errors wrapped into `Error`); the Guardian server-copy code is removed; `browseSave` uses the launcher's dialog helper; gameart: a save the strict read rejects is read again accepting any item id (stacks of mods that are gone), with a read-only note (saving still refuses unknown ids); through `readCharacter` since 2026-11-20, which also opens read-only a save with a mod's own data (Belt Lantern) or an inventory whose place is a guess; `writeSave` no longer calls `backup_file` before `write_save`: the launcher's `write_save` copies the save to the editor's backups by itself, so each save made two identical backups |
| `src/gamedata.js` | amounts: `newEntry` bounds a stacking item's amount by what the save holds (65535), not by the vanilla stack size (a mod may have raised it; `kiln/amounts.js`). gameart: `mergeModded(overlay, version)` merges the items of Kiln mods (the launcher's `modded/items.json`) into `items`/extras (copies of the base item's entry with their own name, rarity, stack, icon; icon overrides of vanilla items; undoable); `itemName` of an id with no entry: "Unknown item (0x…)"; `newEntry` gives a new item its own rarity from `items.json` (the one the catalog shows), not the `maxUpgrades` of the instance `item-extras.json` was read from (rolled loot: Ring of Frost, a Legendary item, was added as Rare) |
| `src/itemstats.js` | gameart: `copiarEstatisticas(de, para)`: a mod item uses its base item's attribute lines |
| `src/ui.js` | gameart: `itemIcon` uses an entry's own `icon` (mod items) when it has one |
| `src/Catalog.jsx` | gameart: the item list is built when the catalog opens (mod items merge after the module loads) |
| `shared/character-exact.mjs`, `shared/storage-witness.mjs`, `shared/player-storage.mjs` | Kiln's exact inventory codec and KPS1 witness, separate from the heuristic parser. `src/tauri-api.js` reads the selected install's storage sidecar by the character's actual inventory witness and returns storage offline through backed-up, sidecar-first writes; the regular editor parser remains unchanged. `kiln/Storage.jsx` is the read-only mirror and explicit return confirmation. |
| `shared/character.mjs` | gameart: when no equipment list of at least 4 pieces fits, the search accepts a shorter one (new characters wear fewer; the inventory header and the mask must still close; checked: byte-identical round trips); 2026-11-20 (player report: empty Backpack): the inventory header's first byte is the bag's page count (1-16, not only 05), entry slots must lie in their container's pages, the first header with entries wins over empty ones (a u32 5 matched an empty header), and `_invUnsure` marks a guessed place; `readCharacter` (strict, then any id accepted) and `modDataIn` (Belt Lantern's record) are Kiln's; `buildCharacter` writes no items into an unsure inventory |
| `src/i18n/en.json` | gameart: "Unknown item (0x{h})", the read-only note for saves with unknown items; Kiln's UI strings (`kiln/`, `deck/`) at the end, with English words for upstream's three messages about a save it cannot read |
| `src/i18n.js` | without a saved choice, anything but Portuguese defaults to English (upstream: Portuguese); outside Portuguese, `t()` words a Portuguese key listed in `src/i18n/launcher-keys.json` with the launcher's text (`launcher/src/i18n`, "editor.game.*") in the launcher's language, and `localeAtual()` is the launcher's locale |
| `src/i18n/launcher-keys.json` | Kiln's: each upstream text the launcher shows (tree checks, save errors, rarity, category and attribute words, skill-tree class labels, knowledge states) → its launcher key; `bun scripts/i18n-check.ts` checks that each key exists |
| `src/styles.css` | scoped under `.kiln-editor` by `launcher/scripts/scope-editor-css.ts` (`:root`/`html`/`body`/`#root` → `.kiln-editor`, every selector prefixed, `.viewport` absolute instead of fixed). Regenerate, don't hand-edit |

Also Kiln's: `deck/` (the Deck's Editor tab, `DeckEditor.jsx`: the same views in the Deck env, the number pad, the
character and backup lists; `deck-editor.css`; docs/kiln/launcher/deck.md "Editor").

Added by Kiln: `KilnEditor.jsx` (wrapper: missing-asset placeholders, the one-line "Preparing game art…", the mod
overlay loaded and merged before a save is read, remounts when the art or the overlay changes), `KilnEditor.d.ts`,
`kiln-editor.css`, this file, and `kiln/`: the editor's UI in the launcher's look (Hearth), which replaced upstream's
in-game frame on 2026-11-05. `kiln/session.js` (the saves, the open character, change, discard, save, restore: one
store for the desktop page and the Deck, so changes survive leaving the page; a save the game wrote after it was read
is refused with Read again when there are changes here, and read again by itself when there are none (on coming back
to the page, and when the game quits); a save read only by accepting unknown ids stays read-only: it shows what it
holds and takes no change), `kiln/pending.ts` (whether
changes are pending, for the shells), `kiln/art.js` (the art and mod-item readiness, shared), `kiln/env.jsx` (desktop
or Deck: the Deck's dialogs, number pad, choice list and hint-bar actions), `kiln/EditorApp.jsx` (page head, character switch, Save, Backups, tabs, keys),
`Equipment.jsx`, `Inventory.jsx`, `Skills.jsx`, `Knowledge.jsx`, `Catalog.jsx` (add an item: the list is sorted once
and filtered per keystroke, and its grid is drawn after the field has answered, the first screens first), `Inspector.jsx` (one
item), `parts.jsx` (the slot, with a neutral square for an item that has no icon), `amounts.js` (how many of an item a slot may hold), `editor.css`. They call upstream's data and save code unchanged (`src/platform.js`,
`src/tauri-api.js`, `src/gamedata.js`, `src/itemstats.js`, `src/treecheck.js`, `src/i18n.js`, `shared/*`) and keep
its rules (equipment mask order, socket bytes, level includes upgrades, the tree checks before a write). Game art
appears only as item and skill icons. Two rules differ: an amount may go above the item's stack size, up to the
65535 the save holds, with a note (a mod may have raised stack sizes, which the editor cannot know; upstream stopped
at the vanilla stack). Level edits use `kiln/levels.js` to keep the total level, including applied upgrades,
within the save's byte (0..255). The vendored `data/items.json` can predate the game's current level ranges and
cannot account for mods, so its `maxLevel` is not an edit limit, including for gems, catalog items and templates.
The instance's own `maxUpgrades` bounds upgrade edits, as upstream's `src/ItemEditor.jsx` describes; no catalog
rarity or fixed five-upgrade limit replaces that saved capacity. Upstream's README documents that the saved level
already includes upgrades: a level-10 hat with 5 upgrades is level 15, not 10.
These limits apply only to edits and newly created items (including templates), never when loading, displaying,
moving or duplicating an existing entry. The write check rejects only non-byte item levels/upgrades, not values
above catalog limits; existing saves remain byte-identical if those fields are not edited. The UI speaks the
launcher's language: Kiln's files call the launcher's `t("editor.…")` (`launcher/src/i18n/*.json`, the `editor` keys, checked
by `bun scripts/i18n-check.ts --source`), upstream's texts go through `src/i18n.js` and `launcher-keys.json`. Item and skill
names come from the locally extracted `names/names.json` in the launcher's language (`kiln/game-names.js`),
then the game's English, then the vendored Portuguese/English tables. Item and recipe knowledge uses the same cache.
Without extraction names behave as before. The Kiln strings at the end of `src/i18n/en.json`
are no longer read (kept for a clean re-vendor diff).

No longer mounted, kept for a clean re-vendor: `src/App.jsx`, `src/Backpack.jsx`, `src/Character.jsx`,
`src/SkillTree.jsx`, `src/Conhecimento.jsx`, `src/Catalog.jsx`, `src/ItemEditor.jsx`, `src/ItemStats.jsx`,
`src/components.jsx`, `src/styles.css`, `registerFonts` in `src/platform.js`.

`shared/recusa.mjs` is unused now (it served the server tab) and stays for a clean re-vendor.

## Game assets
Fonts, item icons and UI textures (upstream `build-assets/game/`, extracted from the game) are game files: never
committed, never bundled. The launcher makes them from the user's installed client into `<data>/editor-assets/`
(`kiln_manage::gameart`, docs/kiln/launcher/gameart.md: the same names and sizes as upstream's set, 4,585 of its 4,587
files). The launcher does it on its own (no button), and adds the items of Kiln mods as an overlay
(`<data>/editor-assets/modded/`). While the art is missing: placeholders and a one-line "Preparing game art…".
`tools/launcher/fetch-editor-assets.sh` copies upstream's set into `launcher/assets-local/editor/` (gitignored; the vite
dev server) as a dev fallback. After re-vendoring, check gameart.md "Editor updates" (texture names, tints, fonts).

## Updating
1. Copy upstream `src`, `shared`, `data` over these folders (keep the Kiln files listed above).
2. Re-apply the table above (`git diff` against this commit shows the exact edits).
3. `bun scripts/scope-editor-css.ts <upstream>/src/styles.css src/editor/src/styles.css`.
4. Update the commit line here.

Kiln changes to the copied files: skill tree class labels go through `t()` with English entries in `src/i18n/en.json`; the catalog shows the other-language item name only to Portuguese readers.
